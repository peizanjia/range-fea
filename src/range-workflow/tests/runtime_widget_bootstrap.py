"""GUI transport fixture only: no solver, FE result or native acceptance evidence.

The Qt test copies this file into an isolated range_workflow package and uses
the real WorkflowProcess NDJSON transport. Acceptance is explicitly staged by
the test, so an enqueue acknowledgement cannot accidentally stand in for it.
"""
import copy
import json
import sys

JOBS = ["00000000-0000-4000-8000-000000000001", "00000000-0000-4000-8000-000000000002"]
counts = {"preview": 0, "approve": 0, "submit": 0, "pause": 0}
proposals = {}
deferred = []
delay_approval = False
execution_state = "armed"
revision = "0"
states = {}


def parameters(gain=2, rev="0"):
    return dict(revision=rev, velocityGainNsm=gain,
                actuatorLimits=[dict(actuatorId="actuator", minN=-100, maxN=100, rateNPerS=0)],
                fixedBaseControllerRevision="b" * 64, parameterSha256=("c" if rev == "0" else "d") * 64,
                runtimeControllerRevision="e" * 64)


def identity(job):
    return dict(jobId=job, runId=job.replace("00000000000", "10000000000"),
                taskId="00000000-0000-4000-8000-000000000003",
                modelSha256="a" * 64, baseControllerRevision="b" * 64)


def attempt(job):
    state = states.setdefault(job, dict(schema="range.structural-runtime-parameters/v1",
                                         parameters=parameters(), history=[]))
    return dict(identity=identity(job), acceptedState=copy.deepcopy(state),
                limits=parameters()["actuatorLimits"], status="ready",
                proposals={key: value for key, value in proposals.items() if value["identity"]["jobId"] == job})


def runtime(job):
    return dict(version=1, attempts={identity(job)["runId"]: attempt(job)})


def reply(request, result):
    print(json.dumps(dict(apiVersion="range.workflow/v1", requestId=request["requestId"],
                         status="ok", code="OK", result=result)), flush=True)


def dispatch(request):
    global delay_approval, execution_state
    op, p = request["operation"], request.get("parameters", {})
    if op == "capabilities.query":
        return dict(apiVersion="range.workflow/v1")
    if op == "fixture.query":
        return dict(counts=counts, deferred=[item[0]["requestId"] for item in deferred])
    if op == "fixture.defer":
        delay_approval = True
        return {}
    if op == "fixture.release":
        for old_request, result in deferred:
            reply(old_request, result)
        deferred.clear()
        delay_approval = False
        return {}
    if op == "fixture.execution":
        execution_state = p["state"]
        return {}
    if op == "fixture.revision":
        states[p["jobId"]]["parameters"] = parameters(rev="1")
        return {}
    if op == "fixture.accept":
        record = proposals[p["proposalId"]]
        receipt = dict(proposalId=record["proposalId"], parameterRevision=record["parameters"]["revision"],
                       parameterSha256=record["parameters"]["parameterSha256"],
                       runtimeControllerRevision=record["parameters"]["runtimeControllerRevision"],
                       status="accepted", reason="", provenance="currentAttempt", appliedSeq="7", acceptedTimeS=.07)
        record.update(status="accepted", receipt=receipt)
        states[record["identity"]["jobId"]] = dict(schema="range.structural-runtime-parameters/v1",
            parameters=record["parameters"], history=[dict(proposalId=record["proposalId"], previousRevision="0",
                revision="1", appliedSeq="7", acceptedTimeS=.07, parameterSha256=receipt["parameterSha256"])])
        return {}
    if op == "jobs.query":
        return dict(jobs=[dict(jobId=job, state="running", runtimeControl=runtime(job)) for job in JOBS])
    if op == "runtime.parameters.query":
        if "proposalId" in p:
            return copy.deepcopy(proposals[p["proposalId"]])
        return dict(jobId=p["jobId"], runtimeControl=runtime(p["jobId"]), connected=True,
                    execution=dict(availability="available", state=execution_state, acceptedSeq="6", timeS=.06))
    if op == "runtime.parameters.preview":
        counts["preview"] += 1
        key = "00000000-0000-4000-8000-" + str(100 + counts["preview"]).zfill(12)
        before = attempt(p["jobId"])["acceptedState"]["parameters"]
        after = parameters(p["velocityGainNsm"], str(int(before["revision"]) + 1))
        after["actuatorLimits"] = p["actuatorLimits"]
        record = dict(proposalId=key, identity=identity(p["jobId"]), beforeParameters=before,
                      parameters=after, status="previewed", previewSha256="f" * 64)
        proposals[key] = record
        return copy.deepcopy(record)
    if op == "host.runtime.parameters.approve":
        counts["approve"] += 1
        result = dict(proposalId=p["proposalId"], approvalToken="fixture-secret-token", previewSha256="f" * 64)
        if delay_approval:
            deferred.append((request, result))
            return None
        return result
    if op == "runtime.parameters.submit":
        assert p["approvalToken"] == "fixture-secret-token"
        counts["submit"] += 1
        proposals[p["proposalId"]]["status"] = "queued"
        return dict(proposalId=p["proposalId"], status="intent", requestId="native-fixture-request")
    if op.startswith("runtime.execution."):
        if op.endswith("pause"):
            counts["pause"] += 1
        return dict(status="requested", requestId="native-fixture-request")
    raise ValueError(op)


for line in sys.stdin:
    request = json.loads(line)
    result = dispatch(request)
    if result is not None:
        reply(request, result)
