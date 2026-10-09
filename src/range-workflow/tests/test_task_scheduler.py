"""Deterministic state and receipt recovery tests; no native jobs are run."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "python"))
from range_workflow.task_plan import PlanError, validate_plan
from range_workflow.task_scheduler import create_state, next_intents, reduce_event, validate_state
from test_task_plan import candidate, catalog

BINDING = {"projectId": "p", **{key: "a" * 64 for key in (
    "sourceSha256", "capabilitySha256", "environmentSha256", "knowledgePackSha256")}}


def initial(raw=None):
    return create_state(validate_plan(raw or candidate(), catalog()), BINDING, "plan", 0)


def event(state, kind, **fields):
    intent = next_intents(state, state["lastAt"])[0]
    return {"type": kind, "nodeId": intent["nodeId"], "intentId": intent["id"],
            "binding": BINDING, **fields}


def submitted():
    state = reduce_event(initial(), {"type": "tick"}, 0)
    state = reduce_event(state, event(state, "preview", previewHash="b" * 64), 0)
    state = reduce_event(state, {"type": "approval", "nodeId": "configure", "previewHash": "b" * 64}, 0)
    return reduce_event(state, {"type": "tick"}, 0)


class TaskSchedulerTests(unittest.TestCase):
    def test_expired_outbox_state_remains_valid_on_restore(self):
        for reason in ("deadlineSeconds", "activeSeconds"):
            with self.subTest(reason=reason):
                raw = candidate()
                raw["budgets"] = {reason: 1}
                state = reduce_event(initial(raw), {"type": "tick"}, 0)
                self.assertTrue(state["intents"])
                expired = reduce_event(state, {"type": "tick"}, 2)
                self.assertEqual(expired["state"], "failed")
                self.assertEqual(expired["intents"], [])
                self.assertTrue(all("intentId" not in node for node in expired["nodes"].values()))
                self.assertEqual(validate_state(expired), expired)

    def test_validate_state_rejects_corrupt_mutable_fields(self):
        def completed_with_pending(state):
            state["state"] = "completed"

        def boolean_count(state):
            state["counts"]["toolCalls"] = True

        def list_count(state):
            state["counts"] = []

        def invalid_nodes(state):
            state["nodes"]["configure"] = []

        def invalid_outbox(state):
            state["intents"] = [{"nodeId": "missing"}]

        def invalid_reference(state):
            state["nodes"]["configure"]["previewHash"] = "not-a-hash"

        def invalid_node_count(state):
            state["nodes"]["configure"]["counts"]["modelCalls"] = True

        for label, corrupt in (("completed with pending nodes", completed_with_pending),
                               ("boolean total count", boolean_count), ("list total count", list_count),
                               ("invalid node map", invalid_nodes), ("invalid outbox owner", invalid_outbox),
                               ("invalid node reference", invalid_reference),
                               ("boolean node count", invalid_node_count)):
            state = initial()
            corrupt(state)
            with self.subTest(case=label), self.assertRaises(PlanError):
                validate_state(state)

    def test_pure_deterministic_reservation_before_dispatch(self):
        original = initial()
        saved = copy.deepcopy(original)
        first = reduce_event(original, {"type": "tick"}, 0)
        self.assertEqual(original, saved)
        self.assertEqual(first, reduce_event(original, {"type": "tick"}, 0))
        self.assertEqual(first["counts"]["toolCalls"], 1)
        self.assertEqual(next_intents(first, 0)[0]["kind"], "preview")
        self.assertEqual(reduce_event(first, {"type": "tick"}, 0), first)

    def test_preview_approval_submission_and_verified_success(self):
        state = submitted()
        self.assertEqual(state["counts"]["toolCalls"], 2)
        state = reduce_event(state, event(state, "queued", jobId="job-original", previewHash="b" * 64), 1)
        state = reduce_event(state, {"type": "tick"}, 2)
        self.assertEqual(next_intents(state, 2)[0]["kind"], "jobQuery")
        with self.assertRaises(PlanError):
            reduce_event(state, {"type": "chargeModel", "nodeId": "configure"}, 2)
        state = reduce_event(state, event(state, "jobObservation", jobId="job-original", status="completed",
            predicate="configure.matches", trustedPredicateSatisfied=True, outputs={"damping": .05}), 1000)
        self.assertEqual(state["nodes"]["configure"]["state"], "completed")
        self.assertEqual(state["activeSeconds"], 1)

    def test_model_or_exit_success_never_satisfies_predicate(self):
        state = submitted()
        state = reduce_event(state, event(state, "queued", jobId="job", previewHash="b" * 64), 0)
        state = reduce_event(state, {"type": "tick"}, 0)
        state = reduce_event(state, event(state, "jobObservation", jobId="job", status="completed",
                                         predicate="configure.matches", trustedPredicateSatisfied=False), 0)
        self.assertEqual(state["state"], "failed")

    def test_reopen_discards_unsubmitted_approval_and_reconciles_submission(self):
        state = reduce_event(initial(), {"type": "tick"}, 0)
        state = reduce_event(state, event(state, "preview", previewHash="b" * 64), 0)
        state = reduce_event(state, {"type": "reopen"}, 1)
        self.assertEqual(state["nodes"]["configure"]["state"], "pending")
        self.assertNotIn("previewHash", state["nodes"]["configure"])
        state = reduce_event(submitted(), {"type": "reopen"}, 1)
        state = reduce_event(state, {"type": "tick"}, 1)
        self.assertEqual(next_intents(state, 1)[0]["kind"], "executionQuery")
        state = reduce_event(state, event(state, "reconcile", status="unobserved", previewHash="b" * 64), 1)
        state = reduce_event(state, {"type": "tick"}, 2)
        self.assertEqual(state["nodes"]["configure"]["state"], "outcome_unknown")
        self.assertEqual(next_intents(state, 2), [])

    def test_reconcile_preserves_original_accepted_job(self):
        state = reduce_event(submitted(), {"type": "reopen"}, 0)
        state = reduce_event(state, {"type": "tick"}, 0)
        state = reduce_event(state, event(state, "reconcile", status="accepted", jobId="original", previewHash="b" * 64), 0)
        self.assertEqual(state["nodes"]["configure"]["jobId"], "original")
        state = reduce_event(state, {"type": "tick"}, 0)
        with self.assertRaises(PlanError):
            reduce_event(state, event(state, "jobObservation", jobId="other", status="running"), 0)

    def test_cancel_stops_dispatch_but_late_accepted_receipt_is_retained(self):
        state = submitted()
        receipt = event(state, "queued", jobId="job", previewHash="b" * 64)
        state = reduce_event(state, {"type": "cancel"}, 0)
        self.assertEqual(next_intents(state, 0), [])
        state = reduce_event(state, receipt, 1)
        self.assertEqual(state["state"], "cancelled")
        self.assertEqual(state["nodes"]["configure"]["jobId"], "job")

    def test_binding_change_conflicts(self):
        state = reduce_event(initial(), {"type": "tick"}, 0)
        receipt = event(state, "preview", previewHash="b" * 64)
        receipt["binding"] = {**BINDING, "sourceSha256": "c" * 64}
        self.assertEqual(reduce_event(state, receipt, 0)["state"], "conflict")

    def test_mutated_immutable_binding_is_rejected(self):
        state = initial()
        state["binding"]["sourceSha256"] = "c" * 64
        with self.assertRaises(PlanError):
            reduce_event(state, {"type": "tick"}, 0)
        with self.assertRaises(PlanError):
            next_intents(state, 0)

    def test_unknown_submit_is_queried_once_without_resubmission(self):
        state = submitted()
        state = reduce_event(state, event(state, "unknown"), 0)
        state = reduce_event(state, {"type": "tick"}, 0)
        self.assertEqual(next_intents(state, 0)[0]["kind"], "executionQuery")
        state = reduce_event(state, event(state, "reconcile", status="unobserved", previewHash="b" * 64), 0)
        state = reduce_event(state, {"type": "reopen"}, 1)
        state = reduce_event(state, {"type": "tick"}, 1)
        self.assertEqual(next_intents(state, 1), [])
        self.assertEqual(state["nodes"]["configure"]["state"], "outcome_unknown")

    def test_budget_and_deadline_counts_survive_reopen(self):
        raw = candidate()
        raw["budgets"] = {"toolCalls": 1}
        state = reduce_event(initial(raw), {"type": "tick"}, 0)
        state = reduce_event(state, {"type": "reopen"}, 0)
        state = reduce_event(state, {"type": "tick"}, 0)
        self.assertEqual(state["state"], "failed")
        self.assertEqual(state["counts"]["toolCalls"], 1)
        state = reduce_event(initial(), {"type": "pause"}, 0)
        state = reduce_event(state, {"type": "resume"}, 86401)
        self.assertEqual(state["state"], "failed")

    def test_failed_independent_required_check_prevents_completion(self):
        raw = {"schema": "range.task-plan/v1", "title": "independent", "nodes": [
            {"id": "q1", "kind": "query", "tool": "jobs.query", "failurePolicy": "continue-independent", "successPredicate": "query.identity"},
            {"id": "q2", "kind": "query", "tool": "jobs.query", "successPredicate": "query.identity"}]}
        state = reduce_event(initial(raw), {"type": "tick"}, 0)
        self.assertEqual(len(next_intents(state, 0)), 2)
        state = reduce_event(state, event(state, "queryResult", status="failed"), 0)
        self.assertEqual(state["state"], "active")
        state = reduce_event(state, event(state, "queryResult", status="completed", predicate="query.identity", trustedPredicateSatisfied=True), 0)
        self.assertEqual(state["state"], "failed")

    def test_late_query_completion_over_node_budget_fails_and_clears_intents(self):
        raw = {"schema": "range.task-plan/v1", "title": "bounded query", "nodes": [
            {"id": "slow", "kind": "query", "tool": "jobs.query", "successPredicate": "query.identity",
             "budgets": {"activeSeconds": 1}}], "budgets": {"activeSeconds": 10}}
        state = reduce_event(initial(raw), {"type": "tick"}, 0)
        self.assertEqual(next_intents(state, 0)[0]["kind"], "query")

        state = reduce_event(state, event(state, "queryResult", status="completed",
            predicate="query.identity", trustedPredicateSatisfied=True, outputs={"jobId": "job"}), 2)

        self.assertEqual(state["nodes"]["slow"]["activeSeconds"], 2)
        self.assertEqual(state["nodes"]["slow"]["state"], "failed")
        self.assertEqual(state["nodes"]["slow"]["failure"], "active time budget exhausted")
        self.assertEqual(state["state"], "failed")
        self.assertEqual(state["intents"], [])
        self.assertEqual(next_intents(state, 2), [])


if __name__ == "__main__":
    unittest.main()
