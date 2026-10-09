"""Host-side runtime approval, correlation, and restore tests."""
import copy
from pathlib import Path
import sys
import threading
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow import runtime_parameters as rp
from range_workflow.runtime_manager import RuntimeManager
from range_workflow.service import WorkflowError


class FakeService:
    def __init__(self, state):
        self._lock = threading.RLock()
        self._closed = False
        self._state = copy.deepcopy(state)
        self.events = []
        self.fail_next_status = None

    def _job(self, job_id):
        return self._state["jobs"][job_id]

    def _commit(self, candidate, command, status, **kwargs):
        if status == self.fail_next_status:
            self.fail_next_status = None
            raise WorkflowError("PERSISTENCE_ERROR", "simulated durable commit failure")
        self.events.append(("commit", command, status))
        self._state = copy.deepcopy(candidate)


class FakeSession:
    def __init__(self, service, job_id, run_id, identity, accepted_state, callback):
        self.service = service
        self.job_id = job_id
        self.run_id = run_id
        self.identity = copy.deepcopy(identity)
        self.accepted_state = copy.deepcopy(accepted_state)
        self.callback = callback
        self.status = "ready"
        self.failure = False
        self.responses = []
        self.threads = []
        self.request_ids = []

    def snapshot(self):
        return {"status": self.status, "identity": copy.deepcopy(self.identity),
                "acceptedState": copy.deepcopy(self.accepted_state)}

    def request(self, operation, parameters=None):
        if self.failure:
            raise ValueError("Native runtime request capacity is full")
        request_id = str(uuid.uuid4())
        self.request_ids.append(request_id)
        self.service.events.append(("request", operation))
        if operation == "enqueue":
            proposal = self.service._state["jobs"][self.job_id]["runtimeControl"]["attempts"][self.run_id]["proposals"][parameters["proposalId"]]
            if proposal["status"] != "intent":
                raise AssertionError("Native request ran before the durable intent commit")
        response = self.responses.pop(0) if self.responses else None
        if response is not None:
            frame = {"op": "reply", "requestId": request_id, "identity": copy.deepcopy(self.identity),
                     "command": operation, "ok": response["ok"], "reason": response["reason"]}
            thread = threading.Thread(target=self.callback, args=(frame,), daemon=True)
            thread.start()
            self.threads.append(thread)
        return request_id

    def join_callbacks(self):
        for thread in self.threads:
            thread.join(2)
            if thread.is_alive():
                raise AssertionError("Runtime callback did not finish")

    def close(self):
        self.status = "closed"


def fixture():
    job_id, run_id, task_id, session_id = (str(uuid.uuid4()) for _ in range(4))
    limits = [{"actuatorId": "force", "minN": -10.0, "maxN": 10.0, "rateNPerS": 5.0}]
    base = "b" * 64
    initial = rp.make_parameters(base, 0, 1.0, limits)
    accepted = {"schema": rp.SCHEMA, "parameters": initial, "history": []}
    identity = {"jobId": job_id, "runId": run_id, "taskId": task_id,
                "modelSha256": "a" * 64, "baseControllerRevision": base}
    attempt = {"identity": copy.deepcopy(identity), "acceptedState": copy.deepcopy(accepted),
               "limits": copy.deepcopy(limits), "status": "ready", "proposals": {}}
    state = {"sessionId": session_id, "jobs": {job_id: {"jobId": job_id, "state": "running",
              "runtimeControl": {"version": 1, "attempts": {run_id: attempt}}}}}
    service = FakeService(state)
    manager = RuntimeManager(service)
    session = FakeSession(service, job_id, run_id, identity, accepted,
                          lambda frame: manager.observe(job_id, run_id, frame))
    manager.sessions[job_id] = (run_id, session)
    return service, manager, session, job_id, run_id


def preview(manager, job_id, gain=2.0):
    return manager.dispatch("runtime.parameters.preview", {"jobId": job_id,
        "velocityGainNsm": gain,
        "actuatorLimits": [{"actuatorId": "force", "minN": -8.0, "maxN": 8.0, "rateNPerS": 4.0}]})


def proposal_record(service, job_id, run_id, proposal_id):
    return service._state["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["proposals"][proposal_id]


def accepted_state_and_receipt(proposal):
    parameters = proposal["parameters"]
    proposal_id = proposal["proposalId"]
    state = {"schema": rp.SCHEMA, "parameters": copy.deepcopy(parameters),
             "history": [{"proposalId": proposal_id, "previousRevision": "0", "revision": "1",
                          "appliedSeq": "5", "acceptedTimeS": 0.5,
                          "parameterSha256": parameters["parameterSha256"]}]}
    receipt = {"proposalId": proposal_id, "parameterRevision": "1",
               "parameterSha256": parameters["parameterSha256"],
               "runtimeControllerRevision": parameters["runtimeControllerRevision"],
               "status": "accepted", "reason": "applied", "provenance": "currentAttempt",
               "appliedSeq": "5", "acceptedTimeS": 0.5}
    return state, receipt


def accept_saved_proposal(service, job_id, run_id, proposal_id):
    attempt = service._state["jobs"][job_id]["runtimeControl"]["attempts"][run_id]
    proposal = attempt["proposals"][proposal_id]
    state, receipt = accepted_state_and_receipt(proposal)
    proposal.update(status="accepted", submittedAt="submitted", receipt=receipt)
    attempt["acceptedState"] = state
    return state, receipt


class RuntimeManagerTest(unittest.TestCase):
    def test_zero_codec_rate_is_accepted_as_source_unlimited(self):
        service, _, _, job_id, run_id = fixture()
        attempt = service._state["jobs"][job_id]["runtimeControl"]["attempts"][run_id]
        unlimited = [{"actuatorId": "force", "minN": -10.0, "maxN": 10.0, "rateNPerS": 0.0}]
        attempt["limits"] = copy.deepcopy(unlimited)
        attempt["acceptedState"]["parameters"] = rp.make_parameters("b" * 64, 0, 1.0, unlimited)

        RuntimeManager(service).restore(service._state)

    def test_explicit_approval_commits_intent_before_request_and_persists_correlation(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        service.events.clear()

        result = manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})

        self.assertEqual(result["status"], "intent")
        self.assertEqual([event[0] for event in service.events], ["commit", "request", "commit"])
        self.assertEqual(service.events[0][2], "intent")
        self.assertEqual(service.events[1], ("request", "enqueue"))
        self.assertEqual(service.events[2][2], "queued")
        self.assertEqual(proposal_record(service, job_id, run_id, proposal["proposalId"])["requestId"], result["requestId"])
        self.assertEqual(manager.requests[result["requestId"]], (job_id, run_id, proposal["proposalId"], "enqueue"))

    def test_correlated_receiptless_negative_enqueue_is_durably_rejected(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        session.responses.append({"ok": False, "reason": "parameter revision changed"})

        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})
        session.join_callbacks()

        saved = proposal_record(service, job_id, run_id, proposal["proposalId"])
        self.assertEqual(saved["status"], "rejected")
        self.assertEqual(saved["error"], "parameter revision changed")
        self.assertEqual(saved["requestId"], session.request_ids[0])
        self.assertNotIn(session.request_ids[0], manager.requests)

    def test_request_id_commit_failure_keeps_correlation_and_never_resends(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        service.fail_next_status = "queued"

        with self.assertRaises(WorkflowError):
            manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
                "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})

        saved = proposal_record(service, job_id, run_id, proposal["proposalId"])
        self.assertEqual(saved["status"], "intent")
        self.assertNotIn("requestId", saved)
        request_id = session.request_ids[0]
        self.assertEqual(manager.requests[request_id], (job_id, run_id, proposal["proposalId"], "enqueue"))
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})
        self.assertEqual(session.request_ids, [request_id])

    def test_receipts_must_match_request_proposal_and_parameter_digests(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})
        request_id = session.request_ids[0]
        receipt = {"proposalId": proposal["proposalId"], "parameterRevision": "1",
                   "parameterSha256": proposal["parameters"]["parameterSha256"],
                   "runtimeControllerRevision": proposal["parameters"]["runtimeControllerRevision"],
                   "status": "queued", "reason": "queued", "provenance": "currentAttempt"}
        frame = {"op": "reply", "requestId": request_id, "identity": copy.deepcopy(session.identity),
                 "command": "enqueue", "ok": True, "reason": "queued", "receipt": copy.deepcopy(receipt)}

        frame["receipt"]["proposalId"] = str(uuid.uuid4())
        with self.assertRaises(ValueError):
            manager.observe(job_id, run_id, frame)
        frame["receipt"] = copy.deepcopy(receipt)
        frame["receipt"]["parameterSha256"] = "c" * 64
        with self.assertRaises(ValueError):
            manager.observe(job_id, run_id, frame)

        frame["receipt"] = receipt
        manager.observe(job_id, run_id, frame)
        self.assertEqual(proposal_record(service, job_id, run_id, proposal["proposalId"])["status"], "queued")
        self.assertNotIn(request_id, manager.requests)

    def test_native_query_receipt_uses_its_own_request_correlation(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})
        result = manager.dispatch("runtime.parameters.query", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        request_id = result["requestId"]
        self.assertEqual(manager.requests[request_id], (job_id, run_id, proposal["proposalId"], "query"))

        receipt = {"proposalId": proposal["proposalId"], "parameterRevision": "1",
                   "parameterSha256": proposal["parameters"]["parameterSha256"],
                   "runtimeControllerRevision": proposal["parameters"]["runtimeControllerRevision"],
                   "status": "queued", "reason": "queued", "provenance": "currentAttempt"}
        manager.observe(job_id, run_id, {"op": "reply", "requestId": request_id,
            "identity": copy.deepcopy(session.identity), "command": "query", "ok": True,
            "reason": "queued", "receipt": receipt})

        self.assertEqual(proposal_record(service, job_id, run_id, proposal["proposalId"])["status"], "queued")
        self.assertNotIn(request_id, manager.requests)

    def test_query_not_found_does_not_reject_unknown_enqueue_or_cross_request_ids(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": approval["approvalToken"]})
        enqueue_id = session.request_ids[0]
        query = manager.dispatch("runtime.parameters.query", {"jobId": job_id,
            "proposalId": proposal["proposalId"]})
        query_id = query["requestId"]

        manager.observe(job_id, run_id, {"op": "reply", "requestId": query_id,
            "identity": copy.deepcopy(session.identity), "command": "query", "ok": False,
            "reason": "NOT_FOUND"})
        self.assertEqual(proposal_record(service, job_id, run_id, proposal["proposalId"])["status"], "intent")
        self.assertIn(enqueue_id, manager.requests)
        self.assertNotIn(query_id, manager.requests)

        manager.observe(job_id, run_id, {"op": "reply", "requestId": enqueue_id,
            "identity": copy.deepcopy(session.identity), "command": "enqueue", "ok": False,
            "reason": "expected revision changed"})
        saved = proposal_record(service, job_id, run_id, proposal["proposalId"])
        self.assertEqual(saved["status"], "rejected")
        self.assertEqual(saved["error"], "expected revision changed")
        self.assertNotIn(enqueue_id, manager.requests)

    def test_local_prequeue_failure_is_rejected_but_lost_response_restores_unknown(self):
        service, manager, session, job_id, run_id = fixture()
        first = preview(manager, job_id)
        first_approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": first["proposalId"]})
        session.failure = True
        result = manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": first["proposalId"], "approvalToken": first_approval["approvalToken"]})
        self.assertEqual(result["status"], "rejected")
        self.assertEqual(proposal_record(service, job_id, run_id, first["proposalId"])["error"], "LOCAL_NOT_SENT")
        self.assertNotIn("requestId", proposal_record(service, job_id, run_id, first["proposalId"]))

        session.failure = False
        second = preview(manager, job_id, gain=3.0)
        second_approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": second["proposalId"]})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": second["proposalId"], "approvalToken": second_approval["approvalToken"]})
        recovered = copy.deepcopy(service._state)
        recovered_service = FakeService(recovered)
        recovered_manager = RuntimeManager(recovered_service)
        recovered_manager.restore(recovered_service._state)
        restored = proposal_record(recovered_service, job_id, run_id, second["proposalId"])
        self.assertEqual(restored["status"], "unknown")
        self.assertEqual(recovered_manager.approvals, {})
        self.assertEqual(recovered_manager.requests, {})
        self.assertEqual(recovered_manager.sessions, {})
        query_result = recovered_manager.dispatch("runtime.parameters.query", {"jobId": job_id,
            "proposalId": second["proposalId"]})
        self.assertEqual(query_result["status"], "unknown")
        self.assertEqual(recovered_manager.sessions, {})
        self.assertEqual(recovered_manager.requests, {})

    def test_restore_expires_preview_and_preserves_only_proven_accepted_record(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        accepted_state, _ = accept_saved_proposal(service, job_id, run_id, proposal["proposalId"])
        session.accepted_state = copy.deepcopy(accepted_state)
        preview_only = preview(manager, job_id, gain=3.0)
        manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": preview_only["proposalId"]})
        candidate = copy.deepcopy(service._state)

        restored_manager = RuntimeManager(service)
        restored_manager.restore(candidate)

        restored_attempt = candidate["jobs"][job_id]["runtimeControl"]["attempts"][run_id]
        self.assertEqual(restored_attempt["status"], "interrupted")
        self.assertEqual(restored_attempt["proposals"][proposal["proposalId"]]["status"], "accepted")
        self.assertEqual(restored_attempt["proposals"][preview_only["proposalId"]]["status"], "expired")
        self.assertEqual(restored_manager.approvals, {})

    def test_restore_rejects_forged_identity_shape_and_parameter_hash_as_corrupt(self):
        service, manager, _, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        self.assertTrue(proposal["proposalId"])
        good = copy.deepcopy(service._state)
        mutations = []

        wrong_identity = copy.deepcopy(good)
        wrong_identity["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["identity"]["taskId"] = str(uuid.uuid4())
        mutations.append(wrong_identity)

        missing_field = copy.deepcopy(good)
        del missing_field["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["identity"]["modelSha256"]
        mutations.append(missing_field)

        bad_hash = copy.deepcopy(good)
        bad_hash["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["proposals"][proposal["proposalId"]]["parameters"]["velocityGainNsm"] = 9.0
        mutations.append(bad_hash)

        forged_accepted = copy.deepcopy(good)
        record = forged_accepted["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["proposals"][proposal["proposalId"]]
        record.update(status="accepted", submittedAt="submitted")
        mutations.append(forged_accepted)

        for index, corrupted in enumerate(mutations):
            with self.subTest(corrupted=index):
                with self.assertRaises(WorkflowError) as raised:
                    RuntimeManager(service).restore(corrupted)
                self.assertEqual(raised.exception.code, "CORRUPT_STATE")

        accepted = copy.deepcopy(good)
        accepted_service = FakeService(accepted)
        accept_saved_proposal(accepted_service, job_id, run_id, proposal["proposalId"])
        accepted = accepted_service._state
        accepted["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["proposals"][proposal["proposalId"]]["receipt"]["appliedSeq"] = "6"
        with self.assertRaises(WorkflowError) as raised:
            RuntimeManager(service).restore(accepted)
        self.assertEqual(raised.exception.code, "CORRUPT_STATE")

    def test_accepted_proposal_duplicate_submit_never_resends_and_stale_preview_is_rejected(self):
        service, manager, session, job_id, run_id = fixture()
        proposal = preview(manager, job_id)
        accept_saved_proposal(service, job_id, run_id, proposal["proposalId"])
        session.accepted_state = copy.deepcopy(service._state["jobs"][job_id]["runtimeControl"]["attempts"][run_id]["acceptedState"])
        result = manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": proposal["proposalId"], "approvalToken": "no-longer-needed"})
        self.assertEqual(result["status"], "accepted")
        self.assertEqual(session.request_ids, [])

        service2, manager2, session2, job_id2, _ = fixture()
        stale = preview(manager2, job_id2)
        approval = manager2.dispatch("host.runtime.parameters.approve", {"jobId": job_id2,
            "proposalId": stale["proposalId"]})
        external_parameters = rp.make_parameters("b" * 64, 1, 1.5,
            [{"actuatorId": "force", "minN": -8.0, "maxN": 8.0, "rateNPerS": 4.0}])
        external_state = {"schema": rp.SCHEMA, "parameters": external_parameters,
            "history": [{"proposalId": str(uuid.uuid4()), "previousRevision": "0", "revision": "1",
                         "appliedSeq": "3", "acceptedTimeS": 0.3,
                         "parameterSha256": external_parameters["parameterSha256"]}]}
        service2._state["jobs"][job_id2]["runtimeControl"]["attempts"][session2.run_id]["acceptedState"] = external_state
        session2.accepted_state = copy.deepcopy(external_state)
        with self.assertRaises(WorkflowError) as stale_approval:
            manager2.dispatch("host.runtime.parameters.approve", {"jobId": job_id2,
                "proposalId": stale["proposalId"]})
        self.assertEqual(stale_approval.exception.code, "RUNTIME_CHANGED")
        with self.assertRaises(WorkflowError) as raised:
            manager2.dispatch("runtime.parameters.submit", {"jobId": job_id2,
                "proposalId": stale["proposalId"], "approvalToken": approval["approvalToken"]})
        self.assertEqual(raised.exception.code, "RUNTIME_CHANGED")

    def test_two_proposals_keep_separate_revision_rejection_correlations(self):
        service, manager, session, job_id, run_id = fixture()
        first = preview(manager, job_id, gain=2.0)
        second = preview(manager, job_id, gain=3.0)
        first_approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": first["proposalId"]})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": first["proposalId"], "approvalToken": first_approval["approvalToken"]})
        second_approval = manager.dispatch("host.runtime.parameters.approve", {"jobId": job_id,
            "proposalId": second["proposalId"]})
        session.responses.append({"ok": False, "reason": "expected revision is stale"})
        manager.dispatch("runtime.parameters.submit", {"jobId": job_id,
            "proposalId": second["proposalId"], "approvalToken": second_approval["approvalToken"]})
        session.join_callbacks()

        self.assertEqual(proposal_record(service, job_id, run_id, first["proposalId"])["status"], "intent")
        rejected = proposal_record(service, job_id, run_id, second["proposalId"])
        self.assertEqual(rejected["status"], "rejected")
        self.assertEqual(rejected["error"], "expected revision is stale")
        self.assertNotEqual(proposal_record(service, job_id, run_id, first["proposalId"])["requestId"],
                            rejected["requestId"])


if __name__ == "__main__":
    unittest.main()
